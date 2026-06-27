#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    enum class QuestRequirementType : u8
    {
        // Quest state checks
        QuestCompleted,  // Target quest must be completed
        QuestActive,     // Target quest must be currently active
        QuestFailed,     // Target quest must have been failed
        QuestNotStarted, // Target quest must not be active, completed, or failed

        // Player attribute checks
        Level,          // Player level comparison (default: >= Value)
        Reputation,     // Faction reputation comparison (Target = faction ID)
        HasTag,         // Player must have a specific tag
        DoesNotHaveTag, // Player must NOT have a specific tag

        // Inventory checks
        HasItem, // Player must have item (Target = item ID, Value = min count)

        // Arbitrary stat checks
        Stat, // Named stat comparison (Target = stat name)

        // Class / race / faction identity
        IsClass,   // Player class must match Target
        IsFaction, // Player faction must match Target

        // Combinators (children-based)
        All, // All children must pass (AND)
        Any, // At least one child must pass (OR)
        Not, // Single child must NOT pass (negation)
    };

    enum class ComparisonOp : u8
    {
        Equal,
        NotEqual,
        GreaterThan,
        GreaterThanOrEqual,
        LessThan,
        LessThanOrEqual,
    };

    [[nodiscard("requirement type string must be used")]] inline const char* RequirementTypeToString(QuestRequirementType type)
    {
        using enum QuestRequirementType;
        switch (type)
        {
            case QuestCompleted:
                return "QuestCompleted";
            case QuestActive:
                return "QuestActive";
            case QuestFailed:
                return "QuestFailed";
            case QuestNotStarted:
                return "QuestNotStarted";
            case Level:
                return "Level";
            case Reputation:
                return "Reputation";
            case HasTag:
                return "HasTag";
            case DoesNotHaveTag:
                return "DoesNotHaveTag";
            case HasItem:
                return "HasItem";
            case Stat:
                return "Stat";
            case IsClass:
                return "IsClass";
            case IsFaction:
                return "IsFaction";
            case All:
                return "All";
            case Any:
                return "Any";
            case Not:
                return "Not";
            default:
                return "Unknown";
        }
    }

    inline std::optional<QuestRequirementType> RequirementTypeFromString(std::string_view str)
    {
        using enum QuestRequirementType;
        if (str == "QuestCompleted")
            return QuestCompleted;
        if (str == "QuestActive")
            return QuestActive;
        if (str == "QuestFailed")
            return QuestFailed;
        if (str == "QuestNotStarted")
            return QuestNotStarted;
        if (str == "Level")
            return Level;
        if (str == "Reputation")
            return Reputation;
        if (str == "HasTag")
            return HasTag;
        if (str == "DoesNotHaveTag")
            return DoesNotHaveTag;
        if (str == "HasItem")
            return HasItem;
        if (str == "Stat")
            return Stat;
        if (str == "IsClass")
            return IsClass;
        if (str == "IsFaction")
            return IsFaction;
        if (str == "All")
            return All;
        if (str == "Any")
            return Any;
        if (str == "Not")
            return Not;
        return std::nullopt;
    }

    [[nodiscard("comparison op string must be used")]] inline const char* ComparisonOpToString(ComparisonOp op)
    {
        using enum ComparisonOp;
        switch (op)
        {
            case Equal:
                return "Equal";
            case NotEqual:
                return "NotEqual";
            case GreaterThan:
                return "GreaterThan";
            case GreaterThanOrEqual:
                return "GreaterThanOrEqual";
            case LessThan:
                return "LessThan";
            case LessThanOrEqual:
                return "LessThanOrEqual";
            default:
                return "GreaterThanOrEqual";
        }
    }

    inline std::optional<ComparisonOp> ComparisonOpFromString(std::string_view str)
    {
        using enum ComparisonOp;
        if (str == "Equal" || str == "==" || str == "EQ")
            return Equal;
        if (str == "NotEqual" || str == "!=" || str == "NE")
            return NotEqual;
        if (str == "GreaterThan" || str == ">" || str == "GT")
            return GreaterThan;
        if (str == "GreaterThanOrEqual" || str == ">=" || str == "GTE")
            return GreaterThanOrEqual;
        if (str == "LessThan" || str == "<" || str == "LT")
            return LessThan;
        if (str == "LessThanOrEqual" || str == "<=" || str == "LTE")
            return LessThanOrEqual;
        return std::nullopt;
    }

    [[nodiscard("comparison result must be used")]] inline bool EvaluateComparison(i32 actual, ComparisonOp op, i32 expected)
    {
        using enum ComparisonOp;
        switch (op)
        {
            case Equal:
                return actual == expected;
            case NotEqual:
                return actual != expected;
            case GreaterThan:
                return actual > expected;
            case GreaterThanOrEqual:
                return actual >= expected;
            case LessThan:
                return actual < expected;
            case LessThanOrEqual:
                return actual <= expected;
            default:
                return false;
        }
    }

    struct QuestRequirement
    {
        QuestRequirementType Type = QuestRequirementType::HasTag;

        // Target identifier (quest ID, tag name, faction ID, item ID, stat name, class name)
        std::string Target;

        // Numeric value for comparisons (min level, reputation threshold, item count, stat value)
        i32 Value = 0;

        // Comparison operator (used by Level, Reputation, Stat, HasItem)
        ComparisonOp Comparison = ComparisonOp::GreaterThanOrEqual;

        // Sub-requirements for combinators (All, Any, Not)
        std::vector<QuestRequirement> Children;

        // Human-readable description for UI tooltip / quest log
        std::string Description;

        auto operator==(const QuestRequirement&) const -> bool = default;
    };

} // namespace OloEngine
