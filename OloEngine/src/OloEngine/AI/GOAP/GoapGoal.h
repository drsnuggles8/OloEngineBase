#pragma once

#include "OloEngine/AI/GOAP/GoapWorldState.h"
#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>

namespace OloEngine
{
    // A desired end-state the agent will try to reach. The agent picks the
    // highest-Priority goal that is currently relevant (CheckValid) and not
    // already satisfied, then asks the planner for a sequence of actions whose
    // combined effects make DesiredState true.
    struct GoapGoal
    {
        std::string Name;
        f32 Priority = 1.0f;         // higher wins when several goals are relevant
        GoapWorldState DesiredState; // the conditions that define success

        // Optional relevance gate. When set and it returns false for the current
        // world state, the goal is skipped entirely this tick (e.g. a "Flee" goal
        // is only valid while "UnderThreat" is true). Unset == always relevant.
        std::function<bool(const GoapWorldState&)> IsValid;

        [[nodiscard]] bool IsSatisfiedBy(const GoapWorldState& state) const
        {
            return state.Satisfies(DesiredState);
        }

        [[nodiscard]] bool CheckValid(const GoapWorldState& state) const
        {
            return !IsValid || IsValid(state);
        }
    };
} // namespace OloEngine
