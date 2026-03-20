#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>
#include <string>
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

    inline const char* RequirementTypeToString(QuestRequirementType type)
    {
        switch (type)
        {
            case QuestRequirementType::QuestCompleted:
                return "QuestCompleted";
            case QuestRequirementType::QuestActive:
                return "QuestActive";
            case QuestRequirementType::QuestFailed:
                return "QuestFailed";
            case QuestRequirementType::QuestNotStarted:
                return "QuestNotStarted";
            case QuestRequirementType::Level:
                return "Level";
            case QuestRequirementType::Reputation:
                return "Reputation";
            case QuestRequirementType::HasTag:
                return "HasTag";
            case QuestRequirementType::DoesNotHaveTag:
                return "DoesNotHaveTag";
            case QuestRequirementType::HasItem:
                return "HasItem";
            case QuestRequirementType::Stat:
                return "Stat";
            case QuestRequirementType::IsClass:
                return "IsClass";
            case QuestRequirementType::IsFaction:
                return "IsFaction";
            case QuestRequirementType::All:
                return "All";
            case QuestRequirementType::Any:
                return "Any";
            case QuestRequirementType::Not:
                return "Not";
            default:
                return "Unknown";
        }
    }

    inline std::optional<QuestRequirementType> RequirementTypeFromString(const std::string& str)
    {
        if (str == "QuestCompleted")
            return QuestRequirementType::QuestCompleted;
        if (str == "QuestActive")
            return QuestRequirementType::QuestActive;
        if (str == "QuestFailed")
            return QuestRequirementType::QuestFailed;
        if (str == "QuestNotStarted")
            return QuestRequirementType::QuestNotStarted;
        if (str == "Level")
            return QuestRequirementType::Level;
        if (str == "Reputation")
            return QuestRequirementType::Reputation;
        if (str == "HasTag")
            return QuestRequirementType::HasTag;
        if (str == "DoesNotHaveTag")
            return QuestRequirementType::DoesNotHaveTag;
        if (str == "HasItem")
            return QuestRequirementType::HasItem;
        if (str == "Stat")
            return QuestRequirementType::Stat;
        if (str == "IsClass")
            return QuestRequirementType::IsClass;
        if (str == "IsFaction")
            return QuestRequirementType::IsFaction;
        if (str == "All")
            return QuestRequirementType::All;
        if (str == "Any")
            return QuestRequirementType::Any;
        if (str == "Not")
            return QuestRequirementType::Not;
        return std::nullopt;
    }

    inline const char* ComparisonOpToString(ComparisonOp op)
    {
        switch (op)
        {
            case ComparisonOp::Equal:
                return "Equal";
            case ComparisonOp::NotEqual:
                return "NotEqual";
            case ComparisonOp::GreaterThan:
                return "GreaterThan";
            case ComparisonOp::GreaterThanOrEqual:
                return "GreaterThanOrEqual";
            case ComparisonOp::LessThan:
                return "LessThan";
            case ComparisonOp::LessThanOrEqual:
                return "LessThanOrEqual";
            default:
                return "GreaterThanOrEqual";
        }
    }

    inline std::optional<ComparisonOp> ComparisonOpFromString(const std::string& str)
    {
        if (str == "Equal" || str == "==" || str == "EQ")
            return ComparisonOp::Equal;
        if (str == "NotEqual" || str == "!=" || str == "NE")
            return ComparisonOp::NotEqual;
        if (str == "GreaterThan" || str == ">" || str == "GT")
            return ComparisonOp::GreaterThan;
        if (str == "GreaterThanOrEqual" || str == ">=" || str == "GTE")
            return ComparisonOp::GreaterThanOrEqual;
        if (str == "LessThan" || str == "<" || str == "LT")
            return ComparisonOp::LessThan;
        if (str == "LessThanOrEqual" || str == "<=" || str == "LTE")
            return ComparisonOp::LessThanOrEqual;
        return std::nullopt;
    }

    inline bool EvaluateComparison(i32 actual, ComparisonOp op, i32 expected)
    {
        switch (op)
        {
            case ComparisonOp::Equal:
                return actual == expected;
            case ComparisonOp::NotEqual:
                return actual != expected;
            case ComparisonOp::GreaterThan:
                return actual > expected;
            case ComparisonOp::GreaterThanOrEqual:
                return actual >= expected;
            case ComparisonOp::LessThan:
                return actual < expected;
            case ComparisonOp::LessThanOrEqual:
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
